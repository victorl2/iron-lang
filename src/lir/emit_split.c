/* emit_split.c -- Split collection struct generation, push/free functions, and prescan.
 *
 * Extracted from emit_c.c and emit_structs.c (Phase 52, Plan 03).
 *
 * Contains:
 *   - emit_prescan_split_collections: module-level scan for interface ARRAY_LITs
 *   - emit_split_arena_helpers: one-shot arena tracking helper emission
 *   - emit_split_collection_for_iface: per-interface struct/push/free emission
 */

#include "lir/emit_split.h"
#include "parser/ast.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>

/* ── Module-level prescan for split collections & layout analysis ─────────── */

void emit_prescan_split_collections(EmitCtx *ctx) {
    if (!ctx->iface_reg) return;

    /* Iterate ALL functions to find interface-typed ARRAY_LITs */
    for (int fi = 0; fi < ctx->module->func_count; fi++) {
        IronLIR_Func *fn = ctx->module->funcs[fi];
        if (!fn || fn->is_extern || fn->block_count == 0) continue;
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
                    hmput(ctx->split_collection_ids, in2->id,
                          iron_arena_strdup(ctx->arena, im, strlen(im)));
                    /* Phase 48-03: Check for layout annotation override */
                    if (in2->type && in2->type->kind == IRON_TYPE_ARRAY) {
                        if (in2->type->array.layout_hint != 0) {
                            hmput(ctx->layout_overrides, in2->id,
                                  in2->type->array.layout_hint);
                        }
                        if (in2->type->array.is_unordered) {
                            hmput(ctx->unordered_collections, in2->id, true);
                        }
                    }
                }
            }
        }
    }

    /* Run field access analysis on the identified split collections */
    if (ctx->split_collection_ids && hmlen(ctx->split_collection_ids) > 0) {
        /* Convert anonymous struct map to Iron_SplitCollectionId for layout analysis */
        Iron_SplitCollectionId *la_ids = NULL;
        for (ptrdiff_t i = 0; i < hmlen(ctx->split_collection_ids); i++) {
            Iron_SplitCollectionId entry;
            entry.key = ctx->split_collection_ids[i].key;
            entry.value = ctx->split_collection_ids[i].value;
            hmputs(la_ids, entry);
        }
        ctx->layout.arena = ctx->arena;
        iron_layout_analyze(&ctx->layout, ctx->module, la_ids, ctx->iface_reg);
        /* Phase 48-02: SoA/AoS layout selection and common field detection */
        iron_layout_select(&ctx->layout, ctx->module, la_ids, ctx->iface_reg);
        hmfree(la_ids);
    }

    /* Phase 50: Value range analysis for field compression */
    ctx->value_range.arena = ctx->arena;
    iron_vr_analyze(&ctx->value_range, ctx->module, ctx->iface_reg);
}

/* ── Arena-tracked allocation helpers ─────────────────────────────────────── */

void emit_split_arena_helpers(EmitCtx *ctx) {
    iron_strbuf_appendf(&ctx->struct_bodies,
        "/* Phase 50: Arena-tracked allocation helpers for split collections */\n"
        "static inline void *_iron_sl_track(void ***tracked_arr, int *count, int *cap, void *ptr) {\n"
        "    if (!ptr) return NULL;\n"
        "    if (*count >= *cap) {\n"
        "        *cap = *cap ? *cap * 2 : 8;\n"
        "        *tracked_arr = (void **)realloc(*tracked_arr, (size_t)*cap * sizeof(void *));\n"
        "    }\n"
        "    (*tracked_arr)[(*count)++] = ptr;\n"
        "    return ptr;\n"
        "}\n"
        "static inline void *_iron_sl_realloc_tracked(void ***tracked_arr, int *count, int *cap, void *old, size_t sz) {\n"
        "    void *p = realloc(old, sz);\n"
        "    if (!p) return NULL;\n"
        "    if (old) {\n"
        "        for (int i = 0; i < *count; i++) {\n"
        "            if ((*tracked_arr)[i] == old) { (*tracked_arr)[i] = p; return p; }\n"
        "        }\n"
        "    }\n"
        "    return _iron_sl_track(tracked_arr, count, cap, p);\n"
        "}\n"
        "static inline void _iron_sl_free_all(void **tracked, int count) {\n"
        "    for (int i = 0; i < count; i++) free(tracked[i]);\n"
        "    free(tracked);\n"
        "}\n\n");
}

/* ── Per-interface split collection emission ──────────────────────────────── */

void emit_split_collection_for_iface(EmitCtx *ctx, const char *iface_mangled,
                                      Iron_IfaceEntry *entry) {
    Iron_StrBuf *sb = &ctx->struct_bodies;

    /* Build lowercase interface name for C identifiers */
    char iface_lower[256];
    {
        size_t nl = strlen(entry->iface_name);
        if (nl >= sizeof(iface_lower)) nl = sizeof(iface_lower) - 1;
        for (size_t ci2 = 0; ci2 < nl; ci2++)
            iface_lower[ci2] = (char)((entry->iface_name[ci2] >= 'A' &&
                                        entry->iface_name[ci2] <= 'Z')
                ? entry->iface_name[ci2] + 32
                : entry->iface_name[ci2]);
        iface_lower[nl] = '\0';
    }

    /* Phase 48: Collect all split collection ValueIds for this interface.
     * Used to compute the union of used fields across all collections. */
    IronLIR_ValueId *iface_collection_vids = NULL; /* stb_ds array */
    if (ctx->split_collection_ids) {
        for (ptrdiff_t si = 0; si < hmlen(ctx->split_collection_ids); si++) {
            if (strcmp(ctx->split_collection_ids[si].value, iface_mangled) == 0) {
                arrput(iface_collection_vids, ctx->split_collection_ids[si].key);
            }
        }
    }

    /* Phase 48: For each impl type, determine which fields are used
     * across ALL collections of this interface (union semantics). */
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl2 = &entry->impls[j];
        if (!impl2->is_alive || !impl2->decl) continue;
        Iron_ObjectDecl *od = impl2->decl;

        int total_fields = od->field_count;
        int used_fields = 0;
        for (int fi = 0; fi < od->field_count; fi++) {
            Iron_Field *f = (Iron_Field *)od->fields[fi];
            bool any_used = false;
            for (int ci2 = 0; ci2 < (int)arrlen(iface_collection_vids); ci2++) {
                if (iron_layout_is_field_used(&ctx->layout,
                        iface_collection_vids[ci2], f->name)) {
                    any_used = true;
                    break;
                }
            }
            if (any_used) used_fields++;
        }

        /* Emit reduced storage typedef if some fields are dead */
        if (used_fields < total_fields && used_fields > 0 &&
            arrlen(iface_collection_vids) > 0) {
            const char *im = emit_mangle_name(impl2->type_name, ctx->arena);
            iron_strbuf_appendf(sb,
                "/* Phase 48: Reduced storage for %s (%d/%d fields) */\n",
                impl2->type_name, used_fields, total_fields);
            iron_strbuf_appendf(sb, "typedef struct {\n");
            for (int fi = 0; fi < od->field_count; fi++) {
                Iron_Field *f = (Iron_Field *)od->fields[fi];
                bool any_used = false;
                for (int ci2 = 0; ci2 < (int)arrlen(iface_collection_vids); ci2++) {
                    if (iron_layout_is_field_used(&ctx->layout,
                            iface_collection_vids[ci2], f->name)) {
                        any_used = true;
                        break;
                    }
                }
                if (!any_used) continue;
                /* Emit field with C type */
                const char *c_type = "int64_t";
                if (f->type_ann) {
                    Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
                    if (!ta->is_func && !ta->is_nullable && !ta->is_array) {
                        c_type = emit_annotation_to_c(ta->name, ctx);
                    }
                }
                /* Phase 50: Value range compression -- use narrower type if proven safe */
                const char *narrowed = iron_vr_get_narrowed_type(
                    &ctx->value_range, impl2->type_name, f->name);
                if (narrowed) {
                    c_type = narrowed;
                    if (ctx->report_compression) {
                        fprintf(stderr, "note: compressed %s.%s to %s\n",
                            impl2->type_name, f->name, narrowed);
                    }
                }
                iron_strbuf_appendf(sb, "    %s %s;\n", c_type, f->name);
            }
            iron_strbuf_appendf(sb, "} %s_Stor;\n\n", im);
            /* Record this type uses reduced storage */
            shput(ctx->reduced_storage_types, impl2->type_name, true);
        }
    }

    /* Phase 48-02: Check for common fields.
     * Common field shared arrays only apply when ALL alive implementors
     * use AoS layout.  When any type uses SoA, each type stores its own
     * per-field arrays, so common field factoring doesn't help
     * (the per-type counts would differ from the shared count). */
    bool any_soa = false;
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl_chk = &entry->impls[j];
        if (!impl_chk->is_alive) continue;
        IronLayoutKind lk_chk = iron_layout_get_kind(&ctx->layout,
            iface_mangled, impl_chk->type_name);
        if (lk_chk == IRON_LAYOUT_SOA) { any_soa = true; break; }
    }
    CommonField *common_fields = NULL;
    if (!any_soa) {
        common_fields = iron_layout_get_common_fields(
            &ctx->layout, entry->iface_name);
    }

    iron_strbuf_appendf(sb, "/* Split collection for %s */\n", iface_mangled);
    iron_strbuf_appendf(sb, "typedef struct {\n");

    /* Phase 50: Arena tracking fields for bulk deallocation */
    iron_strbuf_appendf(sb, "    void **_tracked;\n");
    iron_strbuf_appendf(sb, "    int _tracked_count;\n");
    iron_strbuf_appendf(sb, "    int _tracked_cap;\n");

    /* Phase 48-02: Common field shared arrays (before per-type arrays) */
    if (common_fields && arrlen(common_fields) > 0) {
        iron_strbuf_appendf(sb, "    /* Common fields shared across all implementors */\n");
        for (int cfi = 0; cfi < (int)arrlen(common_fields); cfi++) {
            iron_strbuf_appendf(sb, "    %s *%s_%s;\n",
                common_fields[cfi].c_type, iface_lower,
                common_fields[cfi].name);
        }
        iron_strbuf_appendf(sb, "    int64_t %s_common_count;\n", iface_lower);
        iron_strbuf_appendf(sb, "    int64_t %s_common_cap;\n", iface_lower);
    }

    /* Per-type sub-arrays */
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl2 = &entry->impls[j];
        if (!impl2->is_alive) continue;
        const char *im = emit_mangle_name(impl2->type_name, ctx->arena);
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

        /* Phase 48-02: Check SoA layout for this type */
        IronLayoutKind lk = iron_layout_get_kind(&ctx->layout,
            iface_mangled, impl2->type_name);

        /* Phase 48-03: Layout annotation override with warning */
        for (int ci4 = 0; ci4 < (int)arrlen(iface_collection_vids); ci4++) {
            ptrdiff_t ov_idx = hmgeti(ctx->layout_overrides,
                iface_collection_vids[ci4]);
            if (ov_idx >= 0) {
                int override_hint = ctx->layout_overrides[ov_idx].value;
                IronLayoutKind override_lk = (override_hint == 1)
                    ? IRON_LAYOUT_SOA : IRON_LAYOUT_AOS;
                if (override_lk != lk) {
                    fprintf(stderr,
                        "warning: 'layout: %s' annotation may reduce performance "
                        "-- compiler analysis suggests %s for %s. "
                        "Annotation honored.\n",
                        override_hint == 1 ? "soa" : "aos",
                        lk == IRON_LAYOUT_SOA ? "SoA" : "AoS",
                        impl2->type_name);
                }
                lk = override_lk;
                break;
            }
        }

        if (lk == IRON_LAYOUT_SOA && impl2->decl) {
            /* SoA: emit separate per-field arrays */
            {
                char soa_key_tmp[768];
                snprintf(soa_key_tmp, sizeof(soa_key_tmp), "%s:%s",
                    iface_mangled, impl2->type_name);
                /* Arena-allocate key so it survives block scope */
                const char *soa_key_str = iron_arena_strdup(ctx->arena,
                    soa_key_tmp, strlen(soa_key_tmp));
                shput(ctx->soa_types, soa_key_str, true);
            }
            iron_strbuf_appendf(sb, "    /* SoA layout for %s */\n",
                impl2->type_name);
            Iron_ObjectDecl *od = impl2->decl;
            for (int fi = 0; fi < od->field_count; fi++) {
                Iron_Field *f = (Iron_Field *)od->fields[fi];
                /* Check if this field is used (dead field elimination) */
                bool any_used = true;
                if (arrlen(iface_collection_vids) > 0) {
                    any_used = false;
                    for (int ci2 = 0; ci2 < (int)arrlen(iface_collection_vids); ci2++) {
                        if (iron_layout_is_field_used(&ctx->layout,
                                iface_collection_vids[ci2], f->name)) {
                            any_used = true;
                            break;
                        }
                    }
                }
                /* Skip common fields if they exist (stored in shared arrays) */
                bool is_common = false;
                if (common_fields) {
                    for (int cfi = 0; cfi < (int)arrlen(common_fields); cfi++) {
                        if (strcmp(common_fields[cfi].name, f->name) == 0 &&
                            common_fields[cfi].position == fi) {
                            is_common = true;
                            break;
                        }
                    }
                }
                if (!any_used) continue;
                if (is_common) continue;
                /* Emit field-specific array */
                const char *c_type = "int64_t";
                if (f->type_ann) {
                    Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
                    if (!ta->is_func && !ta->is_nullable && !ta->is_array) {
                        c_type = emit_annotation_to_c(ta->name, ctx);
                    }
                }
                /* Phase 50: Value range compression for SoA field arrays */
                const char *narrowed_soa = iron_vr_get_narrowed_type(
                    &ctx->value_range, impl2->type_name, f->name);
                if (narrowed_soa) c_type = narrowed_soa;
                iron_strbuf_appendf(sb, "    %s *%s_%s;\n",
                    c_type, lower_name, f->name);
            }
            iron_strbuf_appendf(sb, "    int64_t %s_count;\n", lower_name);
            iron_strbuf_appendf(sb, "    int64_t %s_cap;\n", lower_name);
        } else {
            /* AoS: Use reduced storage type if available, otherwise full struct */
            ptrdiff_t red_idx = shgeti(ctx->reduced_storage_types, impl2->type_name);
            if (red_idx >= 0) {
                iron_strbuf_appendf(sb, "    %s_Stor *%s_items;\n", im, lower_name);
            } else {
                iron_strbuf_appendf(sb, "    %s *%s_items;\n", im, lower_name);
            }
            iron_strbuf_appendf(sb, "    int64_t %s_count;\n", lower_name);
            iron_strbuf_appendf(sb, "    int64_t %s_cap;\n", lower_name);
        }
    }
    /* Phase 48-03: Check if ALL collections of this interface are unordered */
    bool all_unordered = (arrlen(iface_collection_vids) > 0);
    for (int ci2 = 0; ci2 < (int)arrlen(iface_collection_vids); ci2++) {
        if (hmgeti(ctx->unordered_collections, iface_collection_vids[ci2]) < 0) {
            all_unordered = false;
            break;
        }
    }

    /* Order index array (skipped for [T, unordered] collections) */
    if (!all_unordered) {
        iron_strbuf_appendf(sb, "    struct { uint8_t tag; int64_t idx; } *_order;\n");
        iron_strbuf_appendf(sb, "    int64_t _order_count;\n");
        iron_strbuf_appendf(sb, "    int64_t _order_cap;\n");
    }
    iron_strbuf_appendf(sb, "    int64_t _total_count;\n");
    iron_strbuf_appendf(sb, "} Iron_SplitList_%s;\n\n", iface_mangled);

    /* Push functions per type */
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl2 = &entry->impls[j];
        if (!impl2->is_alive) continue;
        const char *im = emit_mangle_name(impl2->type_name, ctx->arena);
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

        /* Phase 48-02: Check if this type uses SoA layout */
        char soa_key[768];
        snprintf(soa_key, sizeof(soa_key), "%s:%s",
            iface_mangled, impl2->type_name);
        bool type_is_soa = (shgeti(ctx->soa_types, soa_key) >= 0);

        ptrdiff_t red_idx = shgeti(ctx->reduced_storage_types, impl2->type_name);

        /* Push function always accepts FULL struct (caller pushes concrete object) */
        iron_strbuf_appendf(sb,
            "static inline void Iron_SplitList_%s_push_%s("
            "Iron_SplitList_%s *_sl, %s _val) {\n",
            iface_mangled, impl2->type_name,
            iface_mangled, im);

        if (type_is_soa && impl2->decl) {
            /* Phase 48-02: SoA push -- grow and copy each field array */
            Iron_ObjectDecl *od = impl2->decl;

            /* Capacity growth (shared across all field arrays) */
            iron_strbuf_appendf(sb,
                "    if (_sl->%s_count >= _sl->%s_cap) {\n"
                "        _sl->%s_cap = _sl->%s_cap ? (int64_t)(_sl->%s_cap * 1.5) : 8;\n",
                lower_name, lower_name,
                lower_name, lower_name, lower_name);
            /* Realloc each used non-common field array */
            for (int fi = 0; fi < od->field_count; fi++) {
                Iron_Field *f = (Iron_Field *)od->fields[fi];
                bool any_used = true;
                if (arrlen(iface_collection_vids) > 0) {
                    any_used = false;
                    for (int ci2 = 0; ci2 < (int)arrlen(iface_collection_vids); ci2++) {
                        if (iron_layout_is_field_used(&ctx->layout,
                                iface_collection_vids[ci2], f->name)) {
                            any_used = true;
                            break;
                        }
                    }
                }
                bool is_common = false;
                if (common_fields) {
                    for (int cfi = 0; cfi < (int)arrlen(common_fields); cfi++) {
                        if (strcmp(common_fields[cfi].name, f->name) == 0 &&
                            common_fields[cfi].position == fi) {
                            is_common = true;
                            break;
                        }
                    }
                }
                if (!any_used || is_common) continue;
                const char *c_type = "int64_t";
                if (f->type_ann) {
                    Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
                    if (!ta->is_func && !ta->is_nullable && !ta->is_array) {
                        c_type = emit_annotation_to_c(ta->name, ctx);
                    }
                }
                /* Phase 50: Use narrowed type for realloc sizeof */
                const char *narrowed_r = iron_vr_get_narrowed_type(
                    &ctx->value_range, impl2->type_name, f->name);
                if (narrowed_r) c_type = narrowed_r;
                iron_strbuf_appendf(sb,
                    "        _sl->%s_%s = (%s *)_iron_sl_realloc_tracked("
                    "&_sl->_tracked, &_sl->_tracked_count, &_sl->_tracked_cap, "
                    "_sl->%s_%s, (size_t)_sl->%s_cap * sizeof(%s));\n",
                    lower_name, f->name, c_type,
                    lower_name, f->name,
                    lower_name, c_type);
            }
            iron_strbuf_appendf(sb, "    }\n");

            /* Copy each field to its own array */
            for (int fi = 0; fi < od->field_count; fi++) {
                Iron_Field *f = (Iron_Field *)od->fields[fi];
                bool any_used = true;
                if (arrlen(iface_collection_vids) > 0) {
                    any_used = false;
                    for (int ci2 = 0; ci2 < (int)arrlen(iface_collection_vids); ci2++) {
                        if (iron_layout_is_field_used(&ctx->layout,
                                iface_collection_vids[ci2], f->name)) {
                            any_used = true;
                            break;
                        }
                    }
                }
                bool is_common = false;
                if (common_fields) {
                    for (int cfi = 0; cfi < (int)arrlen(common_fields); cfi++) {
                        if (strcmp(common_fields[cfi].name, f->name) == 0 &&
                            common_fields[cfi].position == fi) {
                            is_common = true;
                            break;
                        }
                    }
                }
                if (!any_used) continue;
                if (is_common) {
                    /* Common fields pushed to shared array instead */
                    iron_strbuf_appendf(sb,
                        "    /* common field %s -> shared array */\n",
                        f->name);
                    continue;
                }
                /* Phase 50: Narrowing cast for SoA field copy */
                const char *narrowed_sf = iron_vr_get_narrowed_type(
                    &ctx->value_range, impl2->type_name, f->name);
                if (narrowed_sf) {
                    iron_strbuf_appendf(sb,
                        "    _sl->%s_%s[_sl->%s_count] = (%s)_val.%s;\n",
                        lower_name, f->name, lower_name, narrowed_sf, f->name);
                } else {
                    iron_strbuf_appendf(sb,
                        "    _sl->%s_%s[_sl->%s_count] = _val.%s;\n",
                        lower_name, f->name, lower_name, f->name);
                }
            }

            /* Push common fields to shared arrays */
            if (common_fields && arrlen(common_fields) > 0) {
                iron_strbuf_appendf(sb,
                    "    if (_sl->%s_common_count >= _sl->%s_common_cap) {\n"
                    "        _sl->%s_common_cap = _sl->%s_common_cap ? (int64_t)(_sl->%s_common_cap * 1.5) : 8;\n",
                    iface_lower, iface_lower,
                    iface_lower, iface_lower, iface_lower);
                for (int cfi = 0; cfi < (int)arrlen(common_fields); cfi++) {
                    iron_strbuf_appendf(sb,
                        "        _sl->%s_%s = (%s *)_iron_sl_realloc_tracked("
                        "&_sl->_tracked, &_sl->_tracked_count, &_sl->_tracked_cap, "
                        "_sl->%s_%s, (size_t)_sl->%s_common_cap * sizeof(%s));\n",
                        iface_lower, common_fields[cfi].name, common_fields[cfi].c_type,
                        iface_lower, common_fields[cfi].name,
                        iface_lower, common_fields[cfi].c_type);
                }
                iron_strbuf_appendf(sb, "    }\n");
                for (int cfi = 0; cfi < (int)arrlen(common_fields); cfi++) {
                    iron_strbuf_appendf(sb,
                        "    _sl->%s_%s[_sl->%s_common_count] = _val.%s;\n",
                        iface_lower, common_fields[cfi].name,
                        iface_lower, common_fields[cfi].name);
                }
                iron_strbuf_appendf(sb,
                    "    _sl->%s_common_count++;\n", iface_lower);
            }
        } else {
            /* AoS push (original + reduced storage support) */
            /* Build storage type name */
            char stor_type_buf[512];
            const char *stor_type;
            if (red_idx >= 0) {
                snprintf(stor_type_buf, sizeof(stor_type_buf), "%s_Stor", im);
                stor_type = stor_type_buf;
            } else {
                stor_type = im;
            }
            /* Grow type-specific sub-array */
            iron_strbuf_appendf(sb,
                "    if (_sl->%s_count >= _sl->%s_cap) {\n"
                "        _sl->%s_cap = _sl->%s_cap ? (int64_t)(_sl->%s_cap * 1.5) : 8;\n"
                "        _sl->%s_items = (%s *)_iron_sl_realloc_tracked("
                "&_sl->_tracked, &_sl->_tracked_count, &_sl->_tracked_cap, "
                "_sl->%s_items, (size_t)_sl->%s_cap * sizeof(%s));\n"
                "    }\n",
                lower_name, lower_name,
                lower_name, lower_name, lower_name,
                lower_name, stor_type,
                lower_name, lower_name, stor_type);

            /* Phase 48: For reduced storage, copy only used fields */
            if (red_idx >= 0 && impl2->decl) {
                Iron_ObjectDecl *od = impl2->decl;
                for (int fi = 0; fi < od->field_count; fi++) {
                    Iron_Field *f = (Iron_Field *)od->fields[fi];
                    bool any_used = false;
                    for (int ci2 = 0; ci2 < (int)arrlen(iface_collection_vids); ci2++) {
                        if (iron_layout_is_field_used(&ctx->layout,
                                iface_collection_vids[ci2], f->name)) {
                            any_used = true;
                            break;
                        }
                    }
                    if (!any_used) continue;
                    /* Phase 50: Narrowing cast for AoS reduced field copy */
                    const char *narrowed_af = iron_vr_get_narrowed_type(
                        &ctx->value_range, impl2->type_name, f->name);
                    if (narrowed_af) {
                        iron_strbuf_appendf(sb,
                            "    _sl->%s_items[_sl->%s_count].%s = (%s)_val.%s;\n",
                            lower_name, lower_name, f->name, narrowed_af, f->name);
                    } else {
                        iron_strbuf_appendf(sb,
                            "    _sl->%s_items[_sl->%s_count].%s = _val.%s;\n",
                            lower_name, lower_name, f->name, f->name);
                    }
                }
            } else {
                /* Store full element */
                iron_strbuf_appendf(sb,
                    "    _sl->%s_items[_sl->%s_count] = _val;\n",
                    lower_name, lower_name);
            }
        }

        /* Grow order index (skipped for unordered collections) */
        if (!all_unordered) {
            iron_strbuf_appendf(sb,
                "    if (_sl->_order_count >= _sl->_order_cap) {\n"
                "        _sl->_order_cap = _sl->_order_cap ? (int64_t)(_sl->_order_cap * 1.5) : 8;\n"
                "        _sl->_order = _iron_sl_realloc_tracked("
                "&_sl->_tracked, &_sl->_tracked_count, &_sl->_tracked_cap, "
                "_sl->_order, (size_t)_sl->_order_cap * sizeof(*_sl->_order));\n"
                "    }\n"
                "    _sl->_order[_sl->_order_count].tag = %d;\n"
                "    _sl->_order[_sl->_order_count].idx = _sl->%s_count;\n"
                "    _sl->_order_count++;\n",
                impl2->tag, lower_name);
        }
        /* Increment counts */
        iron_strbuf_appendf(sb,
            "    _sl->%s_count++;\n"
            "    _sl->_total_count++;\n"
            "}\n\n",
            lower_name);
    }
    /* Free function -- Phase 50: single bulk free via tracked pointer registry */
    iron_strbuf_appendf(sb,
        "static inline void Iron_SplitList_%s_free("
        "Iron_SplitList_%s *_sl) {\n",
        iface_mangled, iface_mangled);
    iron_strbuf_appendf(sb,
        "    _iron_sl_free_all(_sl->_tracked, _sl->_tracked_count);\n");
    iron_strbuf_appendf(sb, "}\n\n");
    arrfree(iface_collection_vids);
}
